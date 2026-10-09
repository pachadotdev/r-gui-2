setwd('~/Documents/r-gui-2')

# these should be listed in the environment pane
x = 1
y <- 1

iris$Sepal.Length

tinyplot::tinyplot(Sepal.Length ~ Petal.Length | Species, data = iris)

png(filename = "my_plot.png", width = 800, height = 600, res = 100)
tinyplot::tinyplot(Sepal.Length ~ Petal.Length | Species, data = iris)
dev.off()

x = list()
x[['a']] = 'a'
x[['b']] = 'b'
x[['a']][['c']] = 'a'

x$a
x$a[['c']]

library(capybara)
